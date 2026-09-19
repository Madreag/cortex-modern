"""In-match chat band: host send appears on the guest, and the band overlaps no occupier.

Writes the network-UI probe scripts the completion pass runs. Does not launch unless --launch
is set. Viewports 640x360, 960x540 and 1280x720. The host send_chat step is the line; the guest
waits for LabelMatchChatNewest within 120 renders; both put a seat message on screen and open the
seats panel, so the layout assertion names two occupiers that are really there, then open the entry
with the chat key and assert what the carved area holds at that size: a history row above the entry
at every supported size, 640x360 included, where the entry itself shrinks to make room for it (see
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
# The row rule holds at every size the game supports; the detector launches the three shortest of them.
SUPPORTED_HEIGHTS = (360, 540, 720, 1080)
CHAT_LINE = "hello from host"
SEAT_MESSAGE = "chat layout occupier"
TICKS = 900
# This detector owns 48700-48705 (two peers per size). The launch driver owns 48320-48539 and
# 48630-48649, the menu readback 48270-48279/48380-48399/48530-48559/48840-48859/49180-49199.
PORT_BASE = 48700


def occupier_steps():
    """The occupiers the band must clear: the seats panel and the seat's own message band."""
    return [
        {"op": "screen_message", "text": SEAT_MESSAGE, "screen": 0, "duration_ms": 60000},
        {"op": "key_down", "key": "F6"},
        {"op": "key_up", "key": "F6"},
        {"op": "wait", "panel_open": True},
        {"op": "wait", "renders": 4, "seat_text_contains": SEAT_MESSAGE, "player": 0},
    ]


def history_row_fits(height):
    """One history row rides above the entry at every supported size, the shortest included.

    With the seats panel open the band lives above it: available = PanelTop(h) - 8, and at 640x360
    PanelTop is F + 32, so available is F + 24 = 36 for the small font's F = 12. A row is lineH = F + 4
    and the entry takes F + 10 where the band still holds a row above it and F + 8 where those two
    pixels are what buys the row, so 640x360 seats 16 + 20 in its 36 exactly and the taller sizes keep
    the roomy entry with rows to spare.
    """
    if height not in SUPPORTED_HEIGHTS:
        raise ValueError(f"{height} is not a supported screen height")
    return True


def open_entry_steps(size):
    """The chat key opens the entry; the band keeps what fits above it and nothing else."""
    width, height = size
    rows = [{"op": "assert_control", "control": "LabelMatchChat0", "equals": {"visible": history_row_fits(height)}}]
    return [
        {"op": "key_down", "key": "CHAT"},
        {"op": "key_up", "key": "CHAT"},
        {"op": "wait", "renders": 4, "chat_entry_open": True},
        {"op": "assert_control", "control": "TextMatchChatInput", "equals": {"visible": True}},
        *rows,
        {"op": "assert_net_ui_clear", "chat_layout": True, "entry_open": True,
         "occupiers": ["seats_panel", "text_band"], "player": 0, "status": False},
        {"op": "key_down", "key": "Escape"},
        {"op": "key_up", "key": "Escape"},
        {"op": "wait", "renders": 4, "chat_entry_open": False},
        {"op": "assert_net_ui_clear", "chat_layout": True, "entry_open": False,
         "occupiers": ["seats_panel", "text_band"], "player": 0, "status": False},
    ]


def host_steps(size):
    return [
        {"op": "wait", "service": "Running"},
        {"op": "wait", "sim_at_least": 60},
        {"op": "send_chat", "text": CHAT_LINE, "scope": "all"},
        {"op": "wait", "renders": 8, "control": "LabelMatchChatNewest", "text_contains": CHAT_LINE},
        {"op": "assert_control", "control": "LabelMatchChatNewest", "text_contains": CHAT_LINE},
        *occupier_steps(),
        *open_entry_steps(size),
        {"op": "finish"},
    ]


def guest_steps(size):
    return [
        {"op": "wait", "service": "Running"},
        {"op": "wait", "sim_at_least": 60},
        {"op": "wait", "renders": 120, "control": "LabelMatchChatNewest", "text_contains": CHAT_LINE},
        {"op": "assert_control", "control": "LabelMatchChatNewest", "text_contains": CHAT_LINE},
        *occupier_steps(),
        *open_entry_steps(size),
        {"op": "finish"},
    ]


def script(steps):
    return {"schema": 1, "timeout_ms": 180000, "steps": steps}


def write_scripts(root: Path) -> dict[str, str]:
    written = {}
    for width, height in SIZES:
        for who, steps in (("host", host_steps((width, height))), ("guest", guest_steps((width, height)))):
            path = root / f"match-chat-{width}x{height}-{who}.json"
            path.write_text(json.dumps(script(steps), indent=2) + "\n", encoding="utf-8")
            written[f"{width}x{height}-{who}"] = str(path)
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


def launch_size(make_run, repo, root, size, port, timeout):
    width, height = size
    case = root / f"{width}x{height}"
    case.mkdir(parents=True, exist_ok=False)
    records, runs = {}, {}
    try:
        for who, steps in (("host", host_steps(size)), ("guest", guest_steps(size))):
            inputs = case / f"{who}_inputs"
            inputs.mkdir()
            probe = inputs / "probe.json"
            probe.write_text(json.dumps(script(steps), indent=2) + "\n", encoding="utf-8")
            env = {"CCCP_HEADLESS": "1", "CC_TEST_NET_UI_SCRIPT": str(probe)}
            runs[who] = make_run(repo, peer_args(who, port, case), case / who, timeout, env=env)
            set_settings(runs[who].cwd, {"ResolutionX": width, "ResolutionY": height, "NetworkChatVisible": 1})

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


def inspect(root: Path, size) -> dict:
    width, height = size
    case = root / f"{width}x{height}"
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
    for index, size in enumerate(SIZES):
        outcome = launch_size(make_run, options.repo.resolve(), root, size,
                              PORT_BASE + index * 2, options.timeout)
        result["sizes"][f"{size[0]}x{size[1]}"] = {"outcome": outcome, "inspect": inspect(root, size)}
    result["pass"] = all(row["inspect"]["pass"] for row in result["sizes"].values())
    (root / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"pass": result["pass"]}, indent=2))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
