"""Acknowledge this headed row's expected warning, retaining its one-dialog assertion."""

from __future__ import annotations

import ctypes as c
import json
import os
from pathlib import Path
import subprocess
import threading


def acknowledge_warning(pid: int, binary: Path, runtime: Path, out: Path, stop: threading.Event, result: dict) -> None:
    try:
        x = c.CDLL("libX11.so.6")
        res = c.CDLL("libXRes.so.1")
        x.XOpenDisplay.argtypes = [c.c_char_p]
        x.XOpenDisplay.restype = c.c_void_p
        x.XCloseDisplay.argtypes = [c.c_void_p]
        x.XQueryTree.argtypes = [c.c_void_p, c.c_ulong, c.POINTER(c.c_ulong), c.POINTER(c.c_ulong),
                                c.POINTER(c.POINTER(c.c_ulong)), c.POINTER(c.c_uint)]
        x.XFree.argtypes = [c.c_void_p]

        class Spec(c.Structure):
            _fields_ = [("client", c.c_ulong), ("mask", c.c_uint)]

        res.XResQueryClientIds.argtypes = [c.c_void_p, c.c_long, c.POINTER(Spec), c.POINTER(c.c_long), c.POINTER(c.c_void_p)]
        res.XResGetClientPid.argtypes = [c.c_void_p]
        res.XResGetClientPid.restype = c.c_int
        res.XResClientIdsDestroy.argtypes = [c.c_long, c.c_void_p]
        env = dict(os.environ, DISPLAY=os.environ.get("DISPLAY", ":0"))
        caption = "RTE Warning! (>_<)"
        process = Path("/proc") / str(pid)

        def owns_dialog(candidate: int | None) -> bool:
            if candidate is None or candidate <= 0:
                return False
            child = Path("/proc") / str(candidate)
            try:
                args = (child / "cmdline").read_bytes().split(b"\0")
                if ((child / "exe").resolve() != binary.resolve()
                        or (child / "cwd").resolve() != runtime.resolve()
                        or b"-module-version-selftest" not in args):
                    return False
                # SDL's X11 message box forks a child which owns the dialog. It must
                # still belong to this runner's engine, with the same binary and cwd.
                seen = set()
                while candidate != pid and candidate > 1 and candidate not in seen:
                    seen.add(candidate)
                    status = (Path("/proc") / str(candidate) / "status").read_text()
                    candidate = int(next(line.split()[1] for line in status.splitlines() if line.startswith("PPid:")))
                return candidate == pid
            except (OSError, ValueError, StopIteration):
                return False
        while not stop.wait(0.1):
            try:
                args = (process / "cmdline").read_bytes().split(b"\0")
                owned = ((process / "exe").resolve() == binary.resolve()
                         and (process / "cwd").resolve() == runtime.resolve()
                         and b"-module-version-selftest" in args)
            except (FileNotFoundError, PermissionError, ProcessLookupError):
                return
            stdout = out / "stdout.log"
            text = stdout.read_text(errors="replace") if stdout.exists() else ""
            if not owned or not all(f"{name}.rte was developed" in text for name in ("StubA", "StubB")):
                continue
            found = subprocess.run(["xdotool", "search", "--name", r"^RTE Warning! \(>_<\)$"],
                                   env=env, capture_output=True, text=True, timeout=5)
            display = x.XOpenDisplay(env["DISPLAY"].encode())
            if not display:
                raise RuntimeError("cannot inspect the headed row's display")
            try:
                windows = []
                for frame in found.stdout.split():
                    window = int(frame)
                    root = c.c_ulong(); parent = c.c_ulong()
                    children = c.POINTER(c.c_ulong)(); count = c.c_uint()
                    x.XQueryTree(display, window, c.byref(root), c.byref(parent), c.byref(children), c.byref(count))
                    windows.extend([window, *(children[i] for i in range(count.value))])
                    if children:
                        x.XFree(children)
                for window in windows:
                    count = c.c_long(); ids = c.c_void_p(); spec = Spec(window, 2)
                    res.XResQueryClientIds(display, 1, c.byref(spec), c.byref(count), c.byref(ids))
                    owner = res.XResGetClientPid(ids) if ids and count.value == 1 else None
                    if ids:
                        res.XResClientIdsDestroy(count, ids)
                    if not owns_dialog(owner):
                        continue
                    title = subprocess.check_output(["xprop", "-id", str(window), "WM_NAME"], env=env, text=True, timeout=5)
                    if f'"{caption}"' not in title:
                        continue
                    subprocess.run(["xdotool", "key", "--window", str(window), "Return"], env=env, check=True, timeout=5)
                    result.update(window=window, engine_pid=pid, owner_pid=owner, runtime=str(runtime), caption=caption,
                                  action="Return", expected_warning_modules=["StubA.rte", "StubB.rte"])
                    (out / "acknowledged-warning.json").write_text(json.dumps(result, indent=2) + "\n")
                    return
            finally:
                x.XCloseDisplay(display)
    except Exception as error:
        result["error"] = str(error)
        (out / "warning-ack-error.json").write_text(json.dumps(result, indent=2) + "\n")


def run_case(repo: Path, out: Path, timeout: float) -> dict:
    from run_sim_test import make_run

    run = make_run(repo, ["-module-version-selftest"], out, timeout)
    stop = threading.Event()
    acknowledgment = {}
    watcher = None
    try:
        run.start()
        assert run.record.get("visible_window"), "this helper is only for a visible Linux row"
        watcher = threading.Thread(target=acknowledge_warning,
                                   args=(run.record["pid"], Path(run.argv[0]), run.cwd, out, stop, acknowledgment), daemon=True)
        watcher.start()
        record = run.finish()
    finally:
        stop.set()
        if watcher:
            watcher.join(timeout=6)
        run.close()
    return {**record, "warning_acknowledgment": acknowledgment}
